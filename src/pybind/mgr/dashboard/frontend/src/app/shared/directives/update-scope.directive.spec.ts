import { AuthStorageService } from '../services/auth-storage.service';
import { PermissionScopeDirective } from './permission-scope.directive';
import { UpdateScopeDirective } from './update-scope.directive';

describe('UpdateScopeDirective', () => {
  it('should create an instance', () => {
    const directive = new UpdateScopeDirective(
      new PermissionScopeDirective(),
      new AuthStorageService(),
      null
    );
    expect(directive).toBeTruthy();
  });
});
