import { Component, EventEmitter, Output } from '@angular/core';
import { MulticlusterService } from '~/app/shared/api/multicluster.service';

@Component({
  selector: 'cd-multicluster-context',
  templateUrl: './multicluster-context.component.html',
  styleUrls: ['./multicluster-context.component.scss']
})
export class MulticlusterContextComponent {
  clusters = [{remoteClusterUrl: 'Select a remote cluster', apiToken: ''}]
  @Output() selectionChanged = new EventEmitter<string>();

  constructor(
    public multiClusterService: MulticlusterService,
  ) {
    
  }

  ngOnInit() {
    this.multiClusterService.getRemoteClusterUrls().subscribe((data : any) => {
      console.log(data);
      this.clusters = this.clusters.concat(data);
    })
  }

  onClusterSelection(value: string) {
    this.multiClusterService.emitSelectionChanged(value);
  }

  }
